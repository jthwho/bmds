/**
 * @file      fmsynth.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The FM synth (devplan/audio.md §26): OP2 banks (the two shipped, and damaged ones), and the
 * synth on banks made here, one instrument at a time: an operator's pitch, the waveforms, the
 * envelope's times against the chip's rates, feedback, velocity, volume, expression and pan,
 * the drums, sustain, stealing, double voices, the resets; every instrument of the real banks.
 */

#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <bmds/decoder.h>
#include <bmds/fmsynth.h>
#include <bmds/mididecoder.h>
#include <bmds/op2bank.h>
#include <bmds/synth.h>

using bmds::ByteSpan;
using bmds::Error;
using bmds::FmSynth;
using bmds::Op2Bank;

namespace {

        constexpr uint32_t Rate = 48000;

        std::vector<uint8_t> readFile(const std::string &path) {
                std::vector<uint8_t> data;
                FILE                *f = std::fopen(path.c_str(), "rb");
                if (f == nullptr) return data;
                uint8_t buf[4096];
                size_t  n;
                while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) data.insert(data.end(), buf, buf + n);
                std::fclose(f);
                return data;
        }

        // An operator's registers, as the bank stores them. By default: multiple 1, sustaining,
        // the fastest attack, no decay, the fastest release, a sine at full level.
        struct OpSpec {
                        uint8_t character = 0x21;
                        uint8_t attackDecay = 0xf0;
                        uint8_t sustainRelease = 0x0f;
                        uint8_t waveform = 0;
                        uint8_t keyScale = 0;
                        uint8_t level = 0;
        };

        // A silent modulator: no attack, so its envelope never leaves silence.
        constexpr OpSpec Silent = {0x21, 0x00, 0x0f, 0, 0, 63};

        struct VoiceSpec {
                        OpSpec  modulator = Silent;
                        OpSpec  carrier;
                        uint8_t feedback = 0;
                        bool    additive = false;
                        int16_t noteOffset = 0;
        };

        // An OP2 bank, every instrument silent until set.
        class BankWriter {
                public:
                        BankWriter() : _bytes(Op2Bank::FullBytes, 0) {
                                std::memcpy(_bytes.data(), "#OPL_II#", 8);
                                for (size_t i = 0; i < Op2Bank::Instruments; ++i) {
                                        VoiceSpec mute;
                                        mute.carrier = Silent;
                                        set(i, mute);
                                }
                        }

                        void set(size_t index, const VoiceSpec &v, uint16_t flags = 0, uint8_t fixedNote = 0,
                                 const VoiceSpec *second = nullptr, uint8_t fineTune = 128) {
                                uint8_t *p = _bytes.data() + Op2Bank::HeaderBytes + index * Op2Bank::InstrumentBytes;
                                p[0] = uint8_t(flags);
                                p[1] = uint8_t(flags >> 8);
                                p[2] = fineTune;
                                p[3] = fixedNote;
                                voice(p + 4, v);
                                voice(p + 20, second != nullptr ? *second : v);
                                return;
                        }

                        const std::vector<uint8_t> &bytes() const { return _bytes; }

                private:
                        static void op(uint8_t *p, const OpSpec &o) {
                                p[0] = o.character;
                                p[1] = o.attackDecay;
                                p[2] = o.sustainRelease;
                                p[3] = o.waveform;
                                p[4] = uint8_t(o.keyScale << 6);
                                p[5] = o.level;
                                return;
                        }
                        static void voice(uint8_t *p, const VoiceSpec &v) {
                                op(p, v.modulator);
                                p[6] = uint8_t(v.feedback << 1 | (v.additive ? 1 : 0));
                                op(p + 7, v.carrier);
                                p[13] = 0;
                                p[14] = uint8_t(v.noteOffset);
                                p[15] = uint8_t(uint16_t(v.noteOffset) >> 8);
                                return;
                        }

                        std::vector<uint8_t> _bytes;
        };

        // A synth on a bank, and its output.
        struct Player {
                        std::vector<uint8_t> bytes;
                        Op2Bank              bank;
                        FmSynth              synth;
                        uint8_t              channels;

                        explicit Player(const std::vector<uint8_t> &b, uint8_t ch = 1, uint16_t voices = 0)
                            : bytes(b), channels(ch) {
                                REQUIRE(bank.open(ByteSpan(bytes.data(), bytes.size())) == Error::Ok);
                                FmSynth::Config c;
                                c.rateHz = Rate;
                                c.channels = ch;
                                c.maxVoices = voices;
                                REQUIRE(synth.open(bank, c) == Error::Ok);
                        }

                        void send(std::initializer_list<uint8_t> message, uint64_t frame = 0) {
                                FmSynth::Event e;
                                e.frame = frame;
                                e.length = uint8_t(message.size());
                                std::copy(message.begin(), message.end(), e.data);
                                REQUIRE(synth.queue(e) == Error::Ok);
                        }

                        std::vector<float> render(size_t frames) {
                                std::vector<float> out(frames * channels);
                                // In uneven pieces, as a stream asks.
                                size_t done = 0;
                                size_t piece = 37;
                                while (done < frames) {
                                        const size_t n = std::min(piece, frames - done);
                                        synth.render(out.data() + done * channels, n);
                                        done += n;
                                        piece = piece * 7 % 301 + 1;
                                }
                                return out;
                        }
        };

        // Rising zero crossings a second, over @p s (mono).
        double frequency(const std::vector<float> &s, size_t from, size_t to) {
                size_t first = 0;
                size_t last = 0;
                int    crossings = 0;
                for (size_t i = from + 1; i < to; ++i) {
                        if (s[i - 1] < 0 && s[i] >= 0) {
                                if (crossings == 0) first = i;
                                last = i;
                                ++crossings;
                        }
                }
                if (crossings < 2) return 0;
                return double(crossings - 1) * Rate / double(last - first);
        }

        double rms(const std::vector<float> &s, size_t from, size_t to, size_t stride = 1, size_t offset = 0) {
                double sum = 0;
                size_t n = 0;
                for (size_t i = from; i < to; ++i) {
                        const double v = s[i * stride + offset];
                        sum += v * v;
                        ++n;
                }
                return n != 0 ? std::sqrt(sum / double(n)) : 0;
        }

        double db(double ratio) {
                return 20.0 * std::log10(ratio);
        }

        // The level in 5 ms windows, in dB below the loudest window, and the first window at or
        // past @p from that crosses @p threshold dB downward (or upward when @p rising).
        double crossingSeconds(const std::vector<float> &s, double threshold, bool rising) {
                constexpr size_t    Window = Rate / 200;
                std::vector<double> levels;
                for (size_t i = 0; i + Window <= s.size(); i += Window) levels.push_back(rms(s, i, i + Window));
                const double peak = *std::max_element(levels.begin(), levels.end());
                for (size_t w = 0; w < levels.size(); ++w) {
                        const double d = levels[w] > 0 ? db(levels[w] / peak) : -200;
                        if (rising ? d >= threshold : (d <= threshold && w > 0)) return double(w * Window) / Rate;
                }
                return -1;
        }

        // The chip's decay steps a chip sample at rate @p rate (Nuked OPL3's schedule).
        double chipSteps(unsigned rate) {
                const unsigned hi = rate >> 2;
                const unsigned lo = rate & 3;
                return std::ldexp((4.0 + lo) / 4.0, int(hi) - 13);
        }

} // namespace

TEST_CASE("Op2Bank: the shipped banks, their instruments and names; damaged ones refused") {
        for (const char *file : {"dmxopl3.op2", "freedoom.op2"}) {
                CAPTURE(file);
                const std::vector<uint8_t> data = readFile(std::string(BMDS_OPL_BANKS_DIR) + "/" + file);
                REQUIRE(data.size() == Op2Bank::FullBytes);
                Op2Bank bank;
                REQUIRE(bank.open(ByteSpan(data.data(), data.size())) == Error::Ok);
                CHECK(bank.name(0) == "Acoustic Grand Piano");
                CHECK(bank.name(Op2Bank::drumIndex(35)) == "Acoustic Bass Drum");
                // The piano: two voices, an octave down, the carrier's waveform an OPL3 one.
                const Op2Bank::Instrument piano = bank.instrument(0);
                CHECK(piano.flags == Op2Bank::DoubleVoice);
                CHECK(piano.fineTune == 130);
                CHECK(piano.voices[0].noteOffset == -12);
                CHECK(piano.voices[0].carrier.waveform == 4);
                CHECK(piano.voices[0].feedback == 7);
                CHECK_FALSE(piano.voices[0].additive);
                CHECK(piano.voices[0].modulator.keyScale == 2);
                CHECK(piano.voices[0].modulator.level == 0x25);
                const Op2Bank::Instrument kick = bank.instrument(Op2Bank::drumIndex(35));
                CHECK((kick.flags & Op2Bank::FixedPitch) != 0);
                CHECK(kick.fixedNote == 25);
        }
        CHECK(Op2Bank::drumIndex(34) == Op2Bank::Instruments);
        CHECK(Op2Bank::drumIndex(81) == Op2Bank::Instruments - 1);
        CHECK(Op2Bank::drumIndex(82) == Op2Bank::Instruments);

        BankWriter                 writer;
        const std::vector<uint8_t> good = writer.bytes();
        Op2Bank                    bank;
        CHECK(bank.open(ByteSpan(good.data(), Op2Bank::MinBytes - 1)) == Error::Invalid);
        CHECK_FALSE(bank.isOpen());
        std::vector<uint8_t> bad = good;
        bad[0] = 'X';
        CHECK(bank.open(ByteSpan(bad.data(), bad.size())) == Error::Invalid);
        // Cut before the names: the instruments, no names.
        REQUIRE(bank.open(ByteSpan(good.data(), Op2Bank::MinBytes)) == Error::Ok);
        CHECK(bank.name(0).isEmpty());
        CHECK(bank.instrument(Op2Bank::Instruments).flags == 0);
        // Any bytes are instruments, masked to their registers.
        std::vector<uint8_t> noise = good;
        for (size_t i = Op2Bank::HeaderBytes; i < noise.size(); ++i) noise[i] = uint8_t(i * 151 + 7);
        REQUIRE(bank.open(ByteSpan(noise.data(), noise.size())) == Error::Ok);
        for (size_t i = 0; i < Op2Bank::Instruments; ++i) {
                const Op2Bank::Instrument in = bank.instrument(i);
                for (const Op2Bank::Voice &v : in.voices) {
                        CHECK(v.carrier.waveform < 8);
                        CHECK(v.modulator.level < 64);
                        CHECK(v.feedback < 8);
                }
                CHECK(bank.name(i).size() <= Op2Bank::NameBytes);
        }
}

TEST_CASE("FmSynth: open's checks; an operator's pitch, its multiple, the bend") {
        BankWriter writer;
        VoiceSpec  sine;
        writer.set(0, sine);
        sine.carrier.character = 0x22; // multiple 2
        writer.set(1, sine);
        sine.carrier.character = 0x20; // multiple ½
        writer.set(2, sine);

        Op2Bank         unopened;
        FmSynth         synth;
        FmSynth::Config c;
        CHECK(synth.open(unopened, c) == Error::Invalid);
        const std::vector<uint8_t> bytes = writer.bytes();
        Op2Bank                    bank;
        REQUIRE(bank.open(ByteSpan(bytes.data(), bytes.size())) == Error::Ok);
        c.rateHz = 7999;
        CHECK(synth.open(bank, c) == Error::Invalid);
        c.rateHz = Rate;
        c.channels = 3;
        CHECK(synth.open(bank, c) == Error::Invalid);
        c.channels = 2;
        c.maxVoices = 257;
        CHECK(synth.open(bank, c) == Error::Invalid);
        c.maxVoices = 0;
        c.gainDb = 13;
        CHECK(synth.open(bank, c) == Error::Invalid);
        c.gainDb = 0;
        REQUIRE(synth.open(bank, c) == Error::Ok);
        CHECK(synth.open(bank, c) == Error::InvalidState);
        CHECK(synth.config().maxVoices == bmds::SynthEngine::DefaultVoices);
        synth.close();
        CHECK_FALSE(synth.isOpen());

        struct Case {
                        uint8_t program;
                        uint8_t key;
                        int     bend; // 14 bits
                        double  hz;
        };
        for (const Case k :
             {Case{0, 69, 8192, 440.0}, Case{1, 69, 8192, 880.0}, Case{2, 69, 8192, 220.0}, Case{0, 57, 8192, 220.0},
              Case{0, 69, 16383, 440.0 * std::pow(2.0, 2.0 / 12)}, Case{0, 69, 0, 440.0 * std::pow(2.0, -2.0 / 12)}}) {
                CAPTURE(int(k.program));
                CAPTURE(int(k.key));
                CAPTURE(k.bend);
                Player p(bytes);
                p.send({0xc0, k.program});
                p.send({0xe0, uint8_t(k.bend & 0x7f), uint8_t(k.bend >> 7)});
                p.send({0x90, k.key, 127});
                const std::vector<float> s = p.render(Rate);
                CHECK(frequency(s, Rate / 10, Rate) == doctest::Approx(k.hz).epsilon(0.002));
        }

        // RPN 0 sets the bend's range: 12 semitones, bent fully up, an octave.
        Player p(bytes);
        p.send({0xb0, 101, 0});
        p.send({0xb0, 100, 0});
        p.send({0xb0, 6, 12});
        p.send({0xe0, 0x7f, 0x7f});
        p.send({0x90, 69, 127});
        const std::vector<float> s = p.render(Rate / 2);
        CHECK(frequency(s, Rate / 10, Rate / 2) ==
              doctest::Approx(440.0 * std::pow(2.0, 8191.0 / 8192)).epsilon(0.002));
}

TEST_CASE("FmSynth: each waveform's shape") {
        struct Shape {
                        double minRatio;  // the most negative sample against the most positive
                        double zeroShare; // the samples at (about) zero; below 0: not checked
        };
        // A low note (55 Hz: 873 frames a cycle), its steady part.
        const Shape shapes[8] = {
                {-1.0, 0.00}, // sine
                {0.0, 0.50},  // half sine: the second half silent
                {0.0, 0.00},  // absolute sine
                {0.0, 0.50},  // quarter sine: the second and fourth quarters silent
                {-1.0, 0.50}, // alternating sine: a full sine in the first half, then silence
                {0.0, 0.50},  // camel sine: two humps in the first half, then silence
                {-1.0, 0.00}, // square
                {-1.0, -1.0}, // logarithmic sawtooth (its quiet ends are many samples near zero)
        };
        for (uint8_t w = 0; w < 8; ++w) {
                CAPTURE(int(w));
                BankWriter writer;
                VoiceSpec  v;
                v.carrier.waveform = w;
                writer.set(0, v);
                Player p(writer.bytes());
                p.send({0x90, 33, 127});
                const std::vector<float> s = p.render(Rate);
                const auto               begin = s.begin() + Rate / 2;
                const float              hi = *std::max_element(begin, s.end());
                const float              lo = *std::min_element(begin, s.end());
                REQUIRE(hi > 0.1f);
                // The chip's negative half is the ones' complement: -1 of 13 bits where it is 0.
                const float tiny = 2.0f / 4096 * hi;
                if (shapes[w].minRatio < 0) {
                        // The sawtooth's peaks are its steepest points, found to a sample: 3 %.
                        CHECK(lo / hi == doctest::Approx(shapes[w].minRatio).epsilon(w == 7 ? 0.03 : 0.01));
                } else {
                        CHECK(lo >= -tiny);
                }
                const double zeros =
                        double(std::count_if(begin, s.end(), [&](float x) { return std::fabs(x) <= tiny; })) /
                        double(s.end() - begin);
                if (shapes[w].zeroShare >= 0) CHECK(zeros == doctest::Approx(shapes[w].zeroShare).epsilon(0.02));
                if (w == 6) {
                        // A square: every sample at the peak, one way or the other.
                        CHECK(std::count_if(begin, s.end(), [&](float x) { return std::fabs(x) < hi * 0.99f; }) == 0);
                }
        }
}

TEST_CASE("FmSynth: the envelope's attack, decay and release against the chip's rates") {
        // The key scale rate off, an A4's block (4) adds 2 to each rate: 4R + 2.
        SUBCASE("decay to -48 dB") {
                for (uint8_t r : {3, 5, 7}) {
                        CAPTURE(int(r));
                        BankWriter writer;
                        VoiceSpec  v;
                        v.carrier.attackDecay = uint8_t(0xf0 | r);
                        v.carrier.sustainRelease = 0xff; // the sustain level at its lowest (93 dB)
                        writer.set(0, v);
                        Player p(writer.bytes());
                        p.send({0x90, 69, 127});
                        const double             expected = 256.0 / chipSteps(4u * r + 2) / 49716.0; // 48 dB: 256 steps
                        const std::vector<float> s = p.render(size_t(expected * 1.5 * Rate) + Rate / 10);
                        CHECK(crossingSeconds(s, -48.0, false) == doctest::Approx(expected).epsilon(0.04).scale(0));
                }
        }
        SUBCASE("release to -48 dB") {
                BankWriter writer;
                VoiceSpec  v;
                v.carrier.sustainRelease = 0x05;
                writer.set(0, v);
                Player p(writer.bytes());
                p.send({0x90, 69, 127});
                p.send({0x80, 69, 0}, Rate / 10);
                const double             expected = 256.0 / chipSteps(4u * 5 + 2) / 49716.0;
                const std::vector<float> s = p.render(size_t(expected * 1.5 * Rate) + Rate / 10);
                CHECK(crossingSeconds(s, -48.0, false) - 0.1 == doctest::Approx(expected).epsilon(0.04).scale(0));
                // Silent by the end of the release, and freed.
                const std::vector<float> after = p.render(size_t(expected * 2 * Rate));
                CHECK(p.synth.stats().voices == 0);
                CHECK(rms(after, after.size() / 2, after.size()) == 0.0);
        }
        SUBCASE("attack to -1 dB") {
                BankWriter writer;
                VoiceSpec  v;
                v.carrier.attackDecay = 0x60; // rate 26: 2^-7 x 1.5 steps a sample
                writer.set(0, v);
                Player p(writer.bytes());
                p.send({0x90, 69, 127});
                const std::vector<float> s = p.render(Rate / 4);
                // (level + 1) from 512 down to 6 (5 steps, 0.94 dB) by 7/8 a step.
                const double steps = std::log2(512.0 / 6.0) / -std::log2(7.0 / 8.0);
                const double expected = steps / chipSteps(26) / 49716.0;
                CHECK(crossingSeconds(s, -5 * 0.1875, true) == doctest::Approx(expected).epsilon(0.1).scale(0));
        }
        SUBCASE("a fast decay stops at the sustain level") {
                // The fastest decay to sustain level 4 (12 dB): the chip stops within a step of it,
                // a tick's steps must not carry it past (found on DMXOPL3's square lead, 12 dB low).
                const auto steady = [](uint8_t attackDecay, uint8_t sustainRelease) {
                        BankWriter writer;
                        VoiceSpec  v;
                        v.carrier.attackDecay = attackDecay;
                        v.carrier.sustainRelease = sustainRelease;
                        writer.set(0, v);
                        Player p(writer.bytes());
                        p.send({0x90, 69, 127});
                        const std::vector<float> s = p.render(Rate / 2);
                        return rms(s, Rate / 4, Rate / 2);
                };
                CHECK(db(steady(0xff, 0x4f) / steady(0xf0, 0x0f)) == doctest::Approx(-12.0).epsilon(0.02));
                CHECK(db(steady(0xff, 0x0f) / steady(0xf0, 0x0f)) == doctest::Approx(0.0).scale(1).epsilon(0.05));
        }
        SUBCASE("percussive: falls from the sustain level at the release rate, keyed on") {
                BankWriter writer;
                VoiceSpec  v;
                v.carrier.character = 0x01; // not sustaining
                v.carrier.sustainRelease = 0x05;
                writer.set(0, v);
                Player p(writer.bytes());
                p.send({0x90, 69, 127});
                const std::vector<float> s = p.render(Rate * 3);
                const double             expected = 256.0 / chipSteps(4u * 5 + 2) / 49716.0;
                CHECK(crossingSeconds(s, -48.0, false) == doctest::Approx(expected).epsilon(0.04).scale(0));
        }
}

TEST_CASE("FmSynth: feedback stays bounded; FM and additive voices") {
        std::vector<float> outs[3];
        for (int k = 0; k < 3; ++k) {
                BankWriter writer;
                VoiceSpec  v;
                v.modulator = OpSpec();
                v.carrier.level = 63; // quiet: the modulator is what is heard
                v.carrier.attackDecay = 0x00;
                v.additive = true;
                v.feedback = uint8_t(k == 0 ? 0 : k == 1 ? 4 : 7);
                writer.set(0, v);
                Player p(writer.bytes());
                p.send({0x90, 60, 127});
                outs[k] = p.render(Rate);
                const float peak = *std::max_element(outs[k].begin(), outs[k].end(),
                                                     [](float a, float b) { return std::fabs(a) < std::fabs(b); });
                CAPTURE(k);
                CHECK(std::isfinite(peak));
                CHECK(std::fabs(peak) <= 4096.0f / 32768.0f * std::pow(10.0f, 13.0f / 20)); // a full voice, calibrated
                CHECK(rms(outs[k], Rate / 2, Rate) > 0.01);
        }
        CHECK(outs[0] != outs[1]);
        CHECK(outs[1] != outs[2]);
        // FM: the modulator moves the carrier's phase, so the sound isn't a sine any more but keeps
        // its pitch.
        BankWriter writer;
        VoiceSpec  fm;
        fm.modulator = OpSpec();
        fm.modulator.level = 20;
        writer.set(0, fm);
        Player p(writer.bytes());
        p.send({0x90, 69, 127});
        const std::vector<float> s = p.render(Rate);
        Player                   pure([] {
                BankWriter w;
                w.set(0, VoiceSpec());
                return w.bytes();
        }());
        pure.send({0x90, 69, 127});
        CHECK(s != pure.render(Rate));
}

TEST_CASE("FmSynth: velocity, volume and expression on GM's curve; pan") {
        BankWriter writer;
        writer.set(0, VoiceSpec());
        const std::vector<uint8_t> bytes = writer.bytes();
        const auto                 level = [&](uint8_t velocity, uint8_t volume, uint8_t expression) {
                Player p(bytes);
                p.send({0xb0, 7, volume});
                p.send({0xb0, 11, expression});
                p.send({0x90, 69, velocity});
                const std::vector<float> s = p.render(Rate / 4);
                return rms(s, Rate / 8, Rate / 4);
        };
        const double full = level(127, 127, 127);
        // 40 log10(v / 127): each in 0.1875 dB steps.
        CHECK(db(level(64, 127, 127) / full) == doctest::Approx(40 * std::log10(64.0 / 127)).epsilon(0.03));
        CHECK(db(level(127, 64, 127) / full) == doctest::Approx(40 * std::log10(64.0 / 127)).epsilon(0.03));
        CHECK(db(level(127, 127, 32) / full) == doctest::Approx(40 * std::log10(32.0 / 127)).epsilon(0.03));
        CHECK(level(127, 0, 127) == 0.0); // 96 dB down: the gain is 0

        // A volume change reaches a sounding note.
        Player v(bytes);
        v.send({0x90, 69, 127});
        v.send({0xb0, 7, 64}, Rate / 4);
        const std::vector<float> s = v.render(Rate / 2);
        CHECK(db(rms(s, Rate * 3 / 8, Rate / 2) / rms(s, Rate / 8, Rate / 4)) ==
              doctest::Approx(40 * std::log10(64.0 / 127) - 40 * std::log10(100.0 / 127)).epsilon(0.03));

        struct Pan {
                        uint8_t value;
                        double  left;
                        double  right;
        };
        for (const Pan k :
             {Pan{0, 1.0, 0.0}, Pan{1, 1.0, 0.0}, Pan{64, std::sqrt(0.5), std::sqrt(0.5)}, Pan{127, 0.0, 1.0}}) {
                CAPTURE(int(k.value));
                Player p(bytes, 2);
                p.send({0xb0, 7, 127});
                p.send({0xb0, 10, k.value});
                p.send({0x90, 69, 127});
                const std::vector<float> st = p.render(Rate / 4);
                CHECK(rms(st, Rate / 8, Rate / 4, 2, 0) / full == doctest::Approx(k.left).epsilon(0.01));
                CHECK(rms(st, Rate / 8, Rate / 4, 2, 1) / full == doctest::Approx(k.right).epsilon(0.01));
        }
}

TEST_CASE("FmSynth: the drum channel's fixed notes; programs") {
        BankWriter writer;
        writer.set(Op2Bank::drumIndex(35), VoiceSpec(), Op2Bank::FixedPitch, 69);
        VoiceSpec octave;
        octave.noteOffset = 12;
        writer.set(5, octave);
        Player p(writer.bytes());
        p.send({0x99, 35, 127}); // channel 10, any key: its fixed note
        p.send({0x99, 34, 127}); // no drum there
        p.send({0x99, 82, 127});
        std::vector<float> s = p.render(Rate / 2);
        CHECK(frequency(s, Rate / 10, Rate / 2) == doctest::Approx(440.0).epsilon(0.002));
        CHECK(p.synth.stats().voices == 1);
        // A program change on channel 10 doesn't change its drums; elsewhere it does.
        p.send({0xc9, 5}, Rate / 2);
        p.send({0x89, 35, 0}, Rate / 2);
        p.send({0xc1, 5}, Rate / 2);
        p.send({0x91, 57, 127}, Rate / 2); // 220 Hz, an octave up by the instrument
        s = p.render(Rate);
        CHECK(frequency(s, Rate / 2, Rate) == doctest::Approx(440.0).epsilon(0.002));
}

TEST_CASE("FmSynth: sustain holds released notes; all notes off, all sound off, reset") {
        BankWriter writer;
        VoiceSpec  v;
        v.carrier.sustainRelease = 0x0a;
        writer.set(0, v);
        const std::vector<uint8_t> bytes = writer.bytes();

        Player p(bytes);
        p.send({0xb0, 64, 127});
        p.send({0x90, 69, 127});
        p.send({0x80, 69, 0}, Rate / 10);
        std::vector<float> s = p.render(Rate / 2);
        CHECK(rms(s, Rate * 4 / 10, Rate / 2) == doctest::Approx(rms(s, Rate / 20, Rate / 10)).epsilon(0.01));
        p.send({0xb0, 64, 0}, Rate / 2);
        s = p.render(Rate / 2);
        CHECK(p.synth.stats().voices == 0);

        // All notes off releases; all sound off ends at once (a few ms).
        Player q(bytes);
        q.send({0x90, 60, 127});
        q.send({0x90, 64, 127});
        q.send({0xb0, 123, 0}, Rate / 10);
        (void)q.render(Rate / 10 + 1);
        CHECK(q.synth.stats().voices == 2);
        (void)q.render(Rate / 2);
        CHECK(q.synth.stats().voices == 0);
        VoiceSpec slow;
        slow.carrier.sustainRelease = 0x01; // a release of a minute
        writer.set(0, slow);
        Player r(writer.bytes());
        r.send({0x90, 60, 127});
        r.send({0x80, 60, 0}, 100);
        r.send({0xb0, 120, 0}, Rate / 10);
        (void)r.render(Rate / 10);
        CHECK(r.synth.stats().voices == 1);
        (void)r.render(Rate / 100);
        CHECK(r.synth.stats().voices == 0);

        // reset(): the same, and the queue forgotten.
        Player t(writer.bytes());
        t.send({0x90, 60, 127});
        t.send({0x90, 62, 127}, Rate);
        (void)t.render(100);
        t.synth.reset();
        CHECK(t.synth.stats().queued == 0);
        (void)t.render(Rate / 50);
        CHECK(t.synth.stats().voices == 0);
        CHECK(t.synth.stats().frames == 100 + Rate / 50);
}

TEST_CASE("FmSynth: the voice limit steals the quietest released voice first; double voices") {
        BankWriter writer;
        VoiceSpec  v;
        v.carrier.sustainRelease = 0x02; // a slow release: still sounding when stolen
        writer.set(0, v);
        VoiceSpec detuned = v;
        writer.set(1, v, Op2Bank::DoubleVoice, 0, &detuned, 192); // the second a semitone up
        const std::vector<uint8_t> bytes = writer.bytes();

        // Three held notes on the right, a released one on the left; a new note on the right takes
        // the released one's voice, so the left goes silent.
        Player p(bytes, 2, 4);
        for (uint8_t ch : {0, 2}) p.send({uint8_t(0xb0 | ch), 10, 127});
        p.send({0xb1, 10, 0});
        p.send({0x91, 50, 127});
        for (uint8_t key : {60, 64, 67}) p.send({0x90, key, 127});
        p.send({0x81, 50, 0}, Rate / 10);
        p.send({0x92, 72, 127}, Rate / 5);
        const std::vector<float> s = p.render(Rate / 2);
        CHECK(rms(s, Rate / 10 + Rate / 20, Rate / 5, 2, 0) > 0.01);
        CHECK(rms(s, Rate / 5 + 64, Rate / 2, 2, 0) == 0.0);
        CHECK(rms(s, Rate / 5 + 64, Rate / 2, 2, 1) > 0.01);
        bmds::SynthEngine::Stats st = p.synth.stats();
        CHECK(st.voices == 4);
        CHECK(st.peakVoices == 4);
        CHECK(st.notesAtLimit == 1);
        // Past the limit with every note held: the oldest goes.
        p.send({0x92, 74, 127}, Rate / 2);
        p.send({0x92, 76, 127}, Rate / 2);
        (void)p.render(64);
        st = p.synth.stats();
        CHECK(st.voices == 4);
        CHECK(st.notesAtLimit == 3);

        // A double-voice instrument takes two voices, the second detuned.
        Player d(bytes);
        d.send({0xc0, 1});
        d.send({0x90, 69, 127});
        (void)d.render(Rate / 10);
        CHECK(d.synth.stats().voices == 2);
        d.send({0x80, 69, 0}, Rate / 10);
        (void)d.render(Rate * 20);
        CHECK(d.synth.stats().voices == 0);
}

TEST_CASE("FmSynth: messages taken and ignored; GM System On; the queue") {
        BankWriter writer;
        writer.set(0, VoiceSpec());
        Player p(writer.bytes());
        p.send({0xf8});         // real-time: nothing
        p.send({0xa0, 60, 10}); // key pressure: taken, unused
        p.send({0xd0, 10});     // channel pressure
        p.send({0xb0, 0, 1});   // bank select
        p.send({0xf1, 0});      // MTC quarter frame: ignored
        p.send({0x90, 60});     // too short
        (void)p.render(64);
        CHECK(p.synth.stats().ignored == 2);
        CHECK(p.synth.stats().voices == 0);

        FmSynth::Event big;
        big.length = 7;
        CHECK(p.synth.queue(big) == Error::Invalid);
        big.length = 0;
        CHECK(p.synth.queue(big) == Error::Invalid);

        // GM System On: programs back to 0.
        Player g([] {
                BankWriter w;
                VoiceSpec  loud;
                w.set(3, loud);
                return w.bytes();
        }());
        g.send({0xc0, 3});
        g.send({0xf0, 0x7e, 0x7f, 0x09, 0x01, 0xf7});
        g.send({0x90, 60, 127});
        const std::vector<float> s = g.render(Rate / 10);
        CHECK(rms(s, 0, s.size()) == 0.0);

        // Full at QueueEvents, counted.
        Player         f(writer.bytes());
        FmSynth::Event e;
        e.length = 3;
        e.data[0] = 0xb0;
        e.data[1] = 7;
        e.data[2] = 100;
        for (size_t i = 0; i < bmds::SynthEngine::QueueEvents; ++i) REQUIRE(f.synth.queue(e) == Error::Ok);
        CHECK(f.synth.queue(e) == Error::Full);
        CHECK(f.synth.stats().queueFull == 1);
        CHECK(f.synth.stats().queued == bmds::SynthEngine::QueueEvents);
}

TEST_CASE("FmSynth: every instrument of the shipped banks through 32 voices") {
        for (const char *file : {"dmxopl3.op2", "freedoom.op2"}) {
                CAPTURE(file);
                Player p(readFile(std::string(BMDS_OPL_BANKS_DIR) + "/" + file), 2);
                // Each program a chord, each drum once, 50 ms apart; then the releases.
                float peak = 0;
                for (int prog = 0; prog < 128 + 40; ++prog) {
                        const uint64_t at = uint64_t(prog) * Rate / 20;
                        if (prog < 128) {
                                p.send({0xc0, uint8_t(prog)}, at);
                                for (uint8_t key : {48, 55, 64, 72}) p.send({0x90, key, 100}, at);
                                for (uint8_t key : {48, 55, 64, 72}) p.send({0x80, key, 0}, at + Rate / 25);
                                p.send({0x99, uint8_t(35 + prog % 47), 127}, at);
                        }
                        for (float x : p.render(Rate / 20)) {
                                REQUIRE(std::isfinite(x));
                                peak = std::max(peak, std::fabs(x));
                        }
                }
                CHECK(peak > 0.05f);
                CHECK(peak < 2.0f);
                CHECK(p.synth.stats().peakVoices <= 32);
        }
}

TEST_CASE("FmSynth: a MIDI file decoded through it, as an engine; the engines' kinds") {
        // The kernel's hooks, given the FM engine on DMXOPL3: what play does with a .mid now.
        static std::vector<uint8_t> bankData;
        static Op2Bank              bank;
        bankData = readFile(std::string(BMDS_OPL_BANKS_DIR) + "/dmxopl3.op2");
        REQUIRE(bank.open(ByteSpan(bankData.data(), bankData.size())) == Error::Ok);
        bmds::MidiDecoder::setSynths(
                [](const bmds::SynthEngine::Config &c) -> bmds::Result<bmds::SynthEngine *> {
                        auto       *s = new FmSynth();
                        const Error err = s->open(bank, c);
                        if (err.isOk()) return bmds::makeResult<bmds::SynthEngine *>(s);
                        delete s;
                        return bmds::makeError<bmds::SynthEngine *>(err);
                },
                [](bmds::SynthEngine *s) { delete s; });
        static bool added = false;
        if (!added) REQUIRE(bmds::Decoder::addFormat("MThd", bmds::MidiDecoder::open) == Error::Ok);
        added = true;
        const std::vector<uint8_t> file = readFile(std::string(BMDS_SYNTH_DIR) + "/scale.mid");
        REQUIRE(!file.empty());
        struct In {
                        const std::vector<uint8_t> *data;
        } in{&file};
        auto readAt = [](void *arg, uint64_t offset, bmds::WritableByteSpan buf) -> Error {
                const auto &d = *static_cast<In *>(arg)->data;
                if (offset > d.size() || d.size() - offset < buf.size()) return Error::EndOfFile;
                std::memcpy(buf.data(), d.data() + offset, buf.size());
                return Error::Ok;
        };
        auto [d, err] = bmds::Decoder::open({readAt, &in, file.size()});
        REQUIRE(err == Error::Ok);
        CHECK(d->info().frames == 336000);
        std::vector<uint8_t> buf(4096 * 8);
        uint64_t             frames = 0;
        float                peak = 0;
        for (;;) {
                auto [n, derr] = d->decode(bmds::WritableByteSpan(buf.data(), buf.size()));
                for (size_t i = 0; i < n / 4; ++i) {
                        float v;
                        std::memcpy(&v, buf.data() + i * 4, 4);
                        REQUIRE(std::isfinite(v));
                        peak = std::max(peak, std::fabs(v));
                }
                frames += n / 8;
                if (derr == Error::EndOfFile) break;
                REQUIRE(derr == Error::Ok);
        }
        CHECK(frames == 336000);
        CHECK(peak > 0.01f);
        bmds::Decoder::close(d);
        bmds::MidiDecoder::setSynths(nullptr, nullptr);

        FmSynth fm;
        CHECK(fm.kind() == bmds::SynthEngine::Kind::Fm);
        CHECK(fm.presets() == Op2Bank::Instruments);
        CHECK(fm.instrumentBytes() == 0); // not open
        bmds::Synth wavetable;
        CHECK(wavetable.kind() == bmds::SynthEngine::Kind::Wavetable);
        CHECK(wavetable.presets() == 0);
}
