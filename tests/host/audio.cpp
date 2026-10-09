/**
 * @file      audio.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Audio's portable parts (devplan/audio.md §16): controls' text, the ES8311
 * clock plan against the user guide's table, its volume mapping, and the
 * test tone's frequency and fades.
 */

#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <bmds/audiocontrol.h>
#include <bmds/audiomix.h>
#include <bmds/audiostream.h>
#include <bmds/es8311regs.h>
#include <bmds/memspace.h>
#include <bmds/resampler.h>
#include <bmds/sampleformat.h>
#include <bmds/sinetone.h>

using bmds::AudioControl;
using bmds::AudioLimiter;
using bmds::ByteSpan;
using bmds::Error;
using bmds::PcmStream;
using bmds::Resampler;
using bmds::SampleFormat;
using bmds::SineTone;
using bmds::SystemSound;
using bmds::ToneSequence;
using bmds::ToneStream;
namespace AudioMix = bmds::AudioMix;
namespace Formats = bmds::SampleFormats;
namespace R = bmds::Es8311Regs;

namespace {

        using Type = AudioControl::Type;
        constexpr AudioControl Slot = {"dac-slot", "", Type::Enum, 0, 1, 1, "left|right"};
        constexpr AudioControl Volume = {"dac-volume", "", Type::Decibel, -9550, 3200, 50, ""};
        constexpr AudioControl Mute = {"dac-mute", "", Type::Bool, 0, 1, 1, ""};
        constexpr AudioControl Ramp = {"dac-ramp", "", Type::Int, 0, 15, 1, ""};

        std::string text(const AudioControl &c, int32_t v) {
                char out[AudioControl::MaxText];
                c.format(v, out, sizeof(out));
                return out;
        }

        // A sine fitted to @p y[from, to) at @p cycles a sample by least squares (cos, sin and a
        // constant): its amplitude, and the rest's power against the sine's in dB (THD+N).
        struct SineFit {
                        double amplitude = 0;
                        double restDb = 0;
        };

        SineFit fitSine(const std::vector<float> &y, double cycles, size_t from, size_t to) {
                double m[3][3] = {};
                double v[3] = {};
                for (size_t i = from; i < to; ++i) {
                        const double w = 2 * M_PI * cycles * static_cast<double>(i);
                        const double b[3] = {std::cos(w), std::sin(w), 1.0};
                        for (int r = 0; r < 3; ++r) {
                                v[r] += b[r] * y[i];
                                for (int c = 0; c < 3; ++c) m[r][c] += b[r] * b[c];
                        }
                }
                // Gaussian elimination (the matrix is well conditioned over many cycles).
                for (int k = 0; k < 3; ++k) {
                        for (int r = k + 1; r < 3; ++r) {
                                const double f = m[r][k] / m[k][k];
                                for (int c = k; c < 3; ++c) m[r][c] -= f * m[k][c];
                                v[r] -= f * v[k];
                        }
                }
                double x[3];
                for (int k = 2; k >= 0; --k) {
                        double sum = v[k];
                        for (int c = k + 1; c < 3; ++c) sum -= m[k][c] * x[c];
                        x[k] = sum / m[k][k];
                }
                double rest = 0;
                for (size_t i = from; i < to; ++i) {
                        const double w = 2 * M_PI * cycles * static_cast<double>(i);
                        const double e = y[i] - (x[0] * std::cos(w) + x[1] * std::sin(w) + x[2]);
                        rest += e * e;
                }
                SineFit fit;
                fit.amplitude = std::sqrt(x[0] * x[0] + x[1] * x[1]);
                const double signal = fit.amplitude * fit.amplitude / 2 * static_cast<double>(to - from);
                fit.restDb = 10 * std::log10(std::max(rest, 1e-30) / signal);
                return fit;
        }

        // The power of @p y[from, to) in dB against full scale's sine.
        double powerDb(const std::vector<float> &y, size_t from, size_t to) {
                double sum = 0;
                for (size_t i = from; i < to; ++i) sum += static_cast<double>(y[i]) * y[i];
                return 10 * std::log10(std::max(sum / static_cast<double>(to - from), 1e-30) / 0.5);
        }

        // A sine of @p hz at @p amplitude, at @p inRate, through a resampler to @p outRate:
        // @p frames output frames of its left channel, in periods of 240 as the engine asks.
        std::vector<float> resampled(uint32_t inRate, uint32_t outRate, double hz, double amplitude, size_t frames) {
                constexpr size_t Period = 240;
                Resampler        r;
                REQUIRE(r.configure(inRate, outRate, Period, bmds::MemSpace::Psram).isOk());
                std::vector<float> out;
                std::vector<float> mix(2 * Period);
                uint64_t           n = 0;
                while (out.size() < frames) {
                        const size_t need = r.inputNeeded(Period);
                        float       *in = r.inputSpace(need);
                        REQUIRE(in != nullptr);
                        for (size_t i = 0; i < need; ++i, ++n) {
                                const auto s = static_cast<float>(
                                        amplitude * std::sin(2 * M_PI * hz * static_cast<double>(n) / inRate));
                                in[2 * i] = s;
                                in[2 * i + 1] = -s;
                        }
                        r.commit(need);
                        std::fill(mix.begin(), mix.end(), 0.0f);
                        const size_t made = r.process(mix.data(), Period, 1.0f);
                        REQUIRE(made == Period);
                        for (size_t i = 0; i < made; ++i) {
                                out.push_back(mix[2 * i]);
                                CHECK_FALSE(mix[2 * i] + mix[2 * i + 1] > 1e-6f); // the right is the left's negative
                        }
                }
                out.resize(frames);
                return out;
        }

} // namespace

TEST_CASE("AudioControl: values read and written as text") {
        CHECK(Slot.parse("right").value() == 1);
        CHECK(Slot.parse("left").value() == 0);
        CHECK(Slot.parse("centre").error() == Error::Invalid);
        CHECK(Slot.parse("").error() == Error::Invalid);
        CHECK(text(Slot, 1) == "right");

        CHECK(Volume.parse("-12.5").value() == -1250);
        CHECK(Volume.parse("-12.5 dB").value() == -1250);
        CHECK(Volume.parse("+3dB").value() == 300);
        CHECK(Volume.parse("0").value() == 0);
        CHECK(Volume.parse(".5").value() == 50);
        CHECK(Volume.parse("-12.25").error() == Error::Invalid); // between steps
        CHECK(Volume.parse("33").error() == Error::OutOfRange);
        CHECK(Volume.parse("-96").error() == Error::OutOfRange);
        CHECK(Volume.parse("1.").error() == Error::Invalid);
        CHECK(Volume.parse("--1").error() == Error::Invalid);
        CHECK(Volume.parse("abc").error() == Error::Invalid);
        CHECK(text(Volume, -1250) == "-12.5 dB");
        CHECK(text(Volume, 0) == "0 dB");
        CHECK(text(Volume, 3200) == "32 dB");
        CHECK(text(Volume, -9550) == "-95.5 dB");

        CHECK(Mute.parse("on").value() == 1);
        CHECK(Mute.parse("No").value() == 0);
        CHECK(Mute.parse("maybe").error() == Error::Invalid);
        CHECK(text(Mute, 1) == "on");

        CHECK(Ramp.parse("15").value() == 15);
        CHECK(Ramp.parse("16").error() == Error::OutOfRange);
        CHECK(Ramp.parse("-1").error() == Error::OutOfRange);
        CHECK(Ramp.parse("1x").error() == Error::Invalid);
        CHECK(std::strcmp(Ramp.typeName(), "int") == 0);
}

TEST_CASE("Es8311: clock plans match the user guide's table") {
        // §8.5's table: MCLK, LRCK, register 0x02.
        struct Row {
                        uint32_t mclk, rate;
                        uint8_t  reg02;
        };
        constexpr Row Guide[] = {
                {24'576'000, 32000, 0x40}, {24'576'000, 48000, 0x20}, {12'288'000, 32000, 0x48},
                {12'288'000, 48000, 0x00}, {18'432'000, 32000, 0x20}, {18'432'000, 48000, 0x48},
                {11'289'600, 44100, 0x00},
        };
        for (const Row &r : Guide) {
                CAPTURE(r.mclk);
                CAPTURE(r.rate);
                auto [plan, err] = R::clockPlan(r.mclk, r.rate);
                REQUIRE(err == Error::Ok);
                CHECK(plan.clkManager2 == r.reg02);
                CHECK(plan.clkManager5 == 0x00);
        }
}

TEST_CASE("Es8311: every rate runs from MCLK = 256 x rate at ratio 256, within the rules") {
        constexpr uint32_t Rates[] = {8000,  11025, 12000, 16000, 22050, 24000,
                                      32000, 44100, 48000, 64000, 88200, 96000};
        for (uint32_t rate : Rates) {
                CAPTURE(rate);
                auto [plan, err] = R::clockPlan(rate * 256, rate);
                REQUIRE(err == Error::Ok);
                CHECK(plan.clkManager2 == 0x00); // ÷1 ×1
                CHECK(plan.ratio == 256);
                CHECK(plan.internalHz == rate * 256);
                CHECK(plan.internalHz <= 35'000'000);
                CHECK(plan.clkManager4 == (rate <= 16000 ? 0x20 : 0x10));
        }
        // Valid plans for odd MCLKs obey the rules too.
        auto [p, e] = R::clockPlan(18'432'000, 8000);
        REQUIRE(e == Error::Ok);
        CHECK(p.ratio % 16 == 0);
        CHECK(p.ratio >= 256);
        CHECK(p.ratio == 288); // ÷8: the smallest ratio of at least 256
        // Out of range, or no setting.
        CHECK(R::clockPlan(7999 * 256, 7999).error() == Error::NotSupported);
        CHECK(R::clockPlan(192000 * 256, 192000).error() == Error::NotSupported);
        CHECK(R::clockPlan(26'000'000, 32000).error() == Error::NotSupported); // not a whole ratio
        CHECK(R::clockPlan(0, 48000).error() == Error::NotSupported);
}

TEST_CASE("Es8311: DAC volume register mapping") {
        CHECK(R::volumeRegister(0) == 0xbf);
        CHECK(R::volumeRegister(-9550) == 0x00);
        CHECK(R::volumeRegister(-20000) == 0x00);
        CHECK(R::volumeRegister(3200) == 0xff);
        CHECK(R::volumeRegister(-50) == 0xbe);
        CHECK(R::volumeRegister(-75) == 0xbd); // rounds toward quieter
        CHECK(R::volumeRegister(150) == 0xc2);
        for (int reg = 0; reg <= 0xff; ++reg)
                CHECK(R::volumeRegister(R::volumeCentiDb(static_cast<uint8_t>(reg))) == reg);
}

TEST_CASE("SineTone: frequency, level, slots and fades") {
        constexpr uint32_t Rate = 48000;
        SineTone           tone;
        CHECK(tone.isFinished());
        tone.start(1000, Rate, -600, SineTone::Left);
        std::vector<float> buf(2 * Rate, 0.0f);
        tone.fill(buf.data(), Rate);
        // Right silent; left crosses zero upward 1000 times a second.
        float peak = 0;
        int   upward = 0;
        int   rightSamples = 0;
        for (uint32_t i = 0; i < Rate; ++i) {
                rightSamples += buf[2 * i + 1] != 0.0f;
                peak = std::max(peak, std::fabs(buf[2 * i]));
                if (i > 0 && buf[2 * (i - 1)] < 0 && buf[2 * i] >= 0) ++upward;
        }
        CHECK(rightSamples == 0);
        CHECK(upward >= 999);
        CHECK(upward <= 1000);
        // -6 dBFS: half of full scale, within a little.
        const double expected = std::pow(10.0, -6.0 / 20.0);
        CHECK(std::fabs(peak - expected) / expected < 0.001);
        // A clean sine: the phasor's rounding stays far below hearing.
        std::vector<float> left(Rate);
        for (uint32_t i = 0; i < Rate; ++i) left[i] = buf[2 * i];
        const SineFit fit = fitSine(left, 1000.0 / Rate, Rate / 10, Rate);
        CHECK(fit.restDb < -100);
        // It faded in: the first sample is near zero.
        CHECK(std::fabs(buf[0]) < expected * 0.01);
        tone.fadeOut();
        CHECK(!tone.isFinished());
        std::fill(buf.begin(), buf.end(), 0.0f);
        tone.fill(buf.data(), Rate / 100); // 10 ms: past the 5 ms fade
        CHECK(tone.isFinished());
        CHECK(buf[2 * (Rate / 100 - 1)] == 0.0f);
}

TEST_CASE("ToneSequence: notes in turn, their lengths, and cancelling") {
        constexpr uint32_t Rate = 48000;
        constexpr size_t   Period = 240;
        // Each system sound ends, within a period of its notes' lengths.
        for (size_t s = 0; s < bmds::SystemSoundCount; ++s) {
                const auto   sound = static_cast<SystemSound>(s);
                ToneSequence seq;
                uint32_t     ms = 0;
                for (const ToneSequence::Note &n : bmds::systemSoundNotes(sound)) ms += n.ms;
                CHECK(ms > 0);
                seq.start(bmds::systemSoundNotes(sound), Rate, SineTone::Both);
                std::vector<float> mix(2 * Period);
                size_t             periods = 0;
                float              peak = 0;
                for (bool more = true; more;) {
                        more = seq.fill(mix.data(), Period);
                        for (float v : mix) peak = std::max(peak, std::fabs(v));
                        std::fill(mix.begin(), mix.end(), 0.0f);
                        ++periods;
                        REQUIRE(periods < 1000);
                }
                CHECK(seq.isFinished());
                const size_t expected = ms * Rate / 1000 / Period;
                CHECK(periods >= expected);
                CHECK(periods <= expected + bmds::systemSoundNotes(sound).size() + 1);
                CHECK(peak > 0.05f);
                CHECK(peak < 0.25f); // no louder than -12 dBFS
                CHECK(std::string(bmds::systemSoundName(sound)).size() > 0);
        }
        // One tone of 100 ms; cancelled after 20 ms, it ends within a period of its fade.
        ToneSequence       seq;
        std::vector<float> mix(2 * Period);
        seq.startTone(500, 100, -1200, Rate, SineTone::Left);
        size_t periods = 0;
        for (bool more = true; more;) {
                more = seq.fill(mix.data(), Period);
                if (++periods == 4) seq.cancel();
                REQUIRE(periods < 100);
        }
        CHECK(periods == 5); // the cancel's fade (5 ms) ends in the fifth
        CHECK_FALSE(seq.fill(mix.data(), Period));
}

TEST_CASE("SampleFormats: names, sizes and exact round trips") {
        for (size_t i = 0; i < Formats::Count; ++i) {
                const auto f = static_cast<SampleFormat>(i);
                auto [parsed, err] = Formats::parse(Formats::name(f));
                REQUIRE(err.isOk());
                CHECK(parsed == f);
                CHECK(Formats::bytesPerSample(f) >= 1);
        }
        CHECK(Formats::parse("s16").error() == Error::Invalid);
        CHECK(Formats::bytesPerSample(SampleFormat::S24_3Le) == 3);
        CHECK(Formats::bytesPerSample(SampleFormat::S24Le) == 4);

        // Every 8- and 16-bit value, and 24-bit ones across the range, to float and back.
        struct Case {
                        SampleFormat format;
                        uint32_t     values; // to try, spread over the range
        };
        const Case cases[] = {
                {SampleFormat::U8, 256},      {SampleFormat::S8, 256},      {SampleFormat::S16Le, 65536},
                {SampleFormat::S16Be, 65536}, {SampleFormat::S24Le, 65536}, {SampleFormat::S24_3Le, 65536},
                {SampleFormat::S32Le, 65536}, {SampleFormat::F32Le, 65536},
        };
        for (const Case &c : cases) {
                const size_t         bytes = Formats::bytesPerSample(c.format);
                std::vector<uint8_t> in(c.values * bytes);
                for (uint32_t v = 0; v < c.values; ++v) {
                        // Spread over the type's range; 32-bit keeps 24 bits (the low byte 0).
                        uint32_t raw = v;
                        if (bytes == 3 || c.format == SampleFormat::S24Le) raw = v * 256 + (v & 0xff);
                        if (c.format == SampleFormat::S24Le)
                                raw = static_cast<uint32_t>(static_cast<int32_t>(raw << 8) >> 8);
                        if (c.format == SampleFormat::S32Le) raw = v << 16 | (v & 0xff) << 8;
                        if (c.format == SampleFormat::F32Le) {
                                const float f = (static_cast<float>(v) - 32768.0f) / 32768.0f;
                                std::memcpy(&raw, &f, 4);
                        }
                        uint8_t *p = &in[v * bytes];
                        if (c.format == SampleFormat::S16Be) {
                                p[0] = static_cast<uint8_t>(raw >> 8);
                                p[1] = static_cast<uint8_t>(raw);
                        } else {
                                for (size_t b = 0; b < bytes; ++b) p[b] = static_cast<uint8_t>(raw >> (8 * b));
                        }
                }
                // As mono (both channels the same), then back to mono.
                std::vector<float> stereo(2 * c.values);
                Formats::toFloat(c.format, 1, in.data(), c.values, stereo.data());
                int outOfRange = 0;
                int unequal = 0;
                for (uint32_t v = 0; v < c.values; ++v) {
                        outOfRange += stereo[2 * v] < -1.0f || stereo[2 * v] >= 1.0f;
                        unequal += stereo[2 * v] != stereo[2 * v + 1];
                }
                CHECK(outOfRange == 0);
                CHECK(unequal == 0);
                std::vector<uint8_t> back(in.size());
                Formats::fromFloat(c.format, 1, stereo.data(), c.values, back.data());
                INFO(Formats::name(c.format));
                CHECK(back == in);
                // And as stereo: the same bytes, two channels a frame.
                std::vector<float> two(c.values);
                Formats::toFloat(c.format, 2, in.data(), c.values / 2, two.data());
                std::vector<uint8_t> back2(c.values / 2 * 2 * bytes);
                Formats::fromFloat(c.format, 2, two.data(), c.values / 2, back2.data());
                CHECK(std::equal(back2.begin(), back2.end(), in.begin()));
        }
        // Known points.
        const uint8_t u8[] = {0x00, 0x80, 0xff};
        float         f[6];
        Formats::toFloat(SampleFormat::U8, 1, u8, 3, f);
        CHECK(f[0] == -1.0f);
        CHECK(f[2] == 0.0f);
        CHECK(f[4] == 127.0f / 128.0f);
        // Clamped: past full scale stays at it.
        const float loud[2] = {1.5f, -1.5f};
        uint8_t     s16[4];
        Formats::fromFloat(SampleFormat::S16Le, 2, loud, 1, s16);
        CHECK(s16[0] == 0xff);
        CHECK(s16[1] == 0x7f);
        CHECK(s16[2] == 0x00);
        CHECK(s16[3] == 0x80);
}

TEST_CASE("SampleFormats: G.711 decodes to the standard's values and encodes back") {
        // The tables' ends and middle (ITU-T G.711).
        CHECK(Formats::muLawToLinear(0x00) == -32124);
        CHECK(Formats::muLawToLinear(0x80) == 32124);
        CHECK(Formats::muLawToLinear(0xff) == 0);
        CHECK(Formats::muLawToLinear(0x7f) == 0);
        CHECK(Formats::aLawToLinear(0xd5) == 8);
        CHECK(Formats::aLawToLinear(0x55) == -8);
        CHECK(Formats::aLawToLinear(0xaa) == 32256);
        CHECK(Formats::aLawToLinear(0x2a) == -32256);
        int muBack = 0;
        int aBack = 0;
        for (int c = 0; c < 256; ++c) {
                const auto code = static_cast<uint8_t>(c);
                // µ-law has two zeros (0x7f, 0xff); both encode as 0xff.
                const uint8_t mu = Formats::linearToMuLaw(Formats::muLawToLinear(code));
                muBack += mu == code || (code == 0x7f && mu == 0xff);
                aBack += Formats::linearToALaw(Formats::aLawToLinear(code)) == code;
        }
        CHECK(muBack == 256);
        CHECK(aBack == 256);
        // Through float, as the engine converts.
        std::vector<uint8_t> codes(256);
        for (int c = 0; c < 256; ++c) codes[c] = static_cast<uint8_t>(c == 0x7f ? 0xff : c);
        for (SampleFormat format : {SampleFormat::MuLaw, SampleFormat::ALaw}) {
                std::vector<float> stereo(512);
                Formats::toFloat(format, 1, codes.data(), 256, stereo.data());
                std::vector<uint8_t> back(256);
                Formats::fromFloat(format, 1, stereo.data(), 256, back.data());
                CHECK(back == codes);
        }
}

TEST_CASE("Resampler: imaging, aliasing, passband and noise, measured") {
        constexpr size_t Frames = 48000;
        constexpr size_t Settle = 4800; // past the filter's delay and the fit's edge
        // Up to 48 kHz from the common rates: a 1 kHz sine comes out clean.
        for (uint32_t in : {8000u, 11025u, 16000u, 22050u, 32000u, 44100u}) {
                const std::vector<float> y = resampled(in, 48000, 1000, 0.5, Frames);
                const SineFit            fit = fitSine(y, 1000.0 / 48000, Settle, Frames);
                INFO("from " << in);
                CHECK(std::fabs(fit.amplitude - 0.5) < 0.5 * 0.002); // within 0.02 dB
                CHECK(fit.restDb < -80);                             // images and noise
        }
        // Down from 96 kHz: a 1 kHz sine passes; 30 kHz (above 24 kHz) is gone, not aliased to 18.
        {
                const std::vector<float> y = resampled(96000, 48000, 1000, 0.5, Frames);
                const SineFit            fit = fitSine(y, 1000.0 / 48000, Settle, Frames);
                CHECK(std::fabs(fit.amplitude - 0.5) < 0.5 * 0.002);
                CHECK(fit.restDb < -80);
                const std::vector<float> a = resampled(96000, 48000, 30000, 0.5, Frames);
                CHECK(powerDb(a, Settle, Frames) < -80);
        }
        // Down from 48 kHz to 8 kHz: 6 kHz is above 4 kHz and must not come out at 2 kHz.
        {
                const std::vector<float> a = resampled(48000, 8000, 6000, 0.5, Frames / 6);
                CHECK(powerDb(a, Settle / 6, Frames / 6) < -80);
        }
        // The passband: flat to 0.75 of the lower Nyquist frequency.
        {
                const std::vector<float> y = resampled(44100, 48000, 16000, 0.5, Frames);
                const SineFit            fit = fitSine(y, 16000.0 / 48000, Settle, Frames);
                CHECK(std::fabs(20 * std::log10(fit.amplitude / 0.5)) < 0.1);
                CHECK(fit.restDb < -70);
        }
        // An image of a tone near the top: 20 kHz from 44.1 kHz images at 24.1 kHz, above 48 kHz's
        // Nyquist frequency, so aliased to 23.9 kHz. The stopband takes it.
        {
                const std::vector<float> y = resampled(44100, 48000, 20000, 0.5, Frames);
                const SineFit            fit = fitSine(y, 20000.0 / 48000, Settle, Frames);
                CHECK(fit.restDb < -40);
        }
}

TEST_CASE("Resampler: exact position over a long run, and short input") {
        // 44.1 to 48 kHz for 10 s of output: the input consumed is exactly 441/480 of it, give or
        // take the filter's length.
        Resampler r;
        REQUIRE(r.configure(44100, 48000, 240, bmds::MemSpace::Psram).isOk());
        std::vector<float> mix(480);
        uint64_t           in = 0;
        for (int p = 0; p < 2000; ++p) {
                const size_t need = r.inputNeeded(240);
                float       *s = r.inputSpace(need);
                REQUIRE(s != nullptr);
                std::fill(s, s + 2 * need, 0.0f);
                r.commit(need);
                in += need;
                CHECK(r.process(mix.data(), 240, 1.0f) == 240);
        }
        const uint64_t expected = 2000ull * 240 * 44100 / 48000;
        CHECK(in >= expected);
        CHECK(in <= expected + 2 * r.delayFrames() + 2);
        // Input short of what's needed: fewer frames made, the rest next time.
        const size_t need = r.inputNeeded(240);
        r.commit(0);
        CHECK(r.process(mix.data(), 240, 1.0f) < 240);
        (void)need;
        CHECK(r.configure(0, 48000, 240, bmds::MemSpace::Psram) == Error::Invalid);
}

TEST_CASE("AudioLimiter: transparent within full scale, no overshoot past it") {
        constexpr uint32_t Rate = 48000;
        AudioLimiter       lim;
        lim.setRate(Rate);
        std::vector<float> buf(2 * Rate);
        for (size_t i = 0; i < Rate; ++i) {
                buf[2 * i] = 0.99f * std::sin(2 * M_PI * 440 * i / Rate);
                buf[2 * i + 1] = -buf[2 * i];
        }
        std::vector<float> copy = buf;
        lim.process(buf.data(), Rate);
        CHECK(buf == copy);
        CHECK(lim.limitedFrames() == 0);
        // Twice full scale: never past it, and back to 1 within a few time constants after.
        for (size_t i = 0; i < Rate; ++i) {
                buf[2 * i] = (i < Rate / 2 ? 2.0f : 0.5f) * std::sin(2 * M_PI * 440 * i / Rate);
                buf[2 * i + 1] = buf[2 * i];
        }
        lim.process(buf.data(), Rate);
        float peak = 0;
        for (float v : buf) peak = std::max(peak, std::fabs(v));
        CHECK(peak <= 1.0f);
        CHECK(lim.limitedFrames() > 0);
        // 0.5 s after: e^-10 of the way from 0.5; another 0.5 s and it is back at 1 exactly.
        CHECK(lim.gain() > 0.9999f);
        std::fill(buf.begin(), buf.end(), 0.0f);
        lim.process(buf.data(), Rate / 2);
        CHECK(lim.gain() == 1.0f);
}

TEST_CASE("AudioMix: slots, mono, clamping and gains") {
        const float stereo[] = {0.5f, -0.5f, 1.0f, 1.0f, -1.0f, -1.0f, 2.0f, -3.0f, 0.25f, 0.75f};
        int32_t     slots[10];
        AudioMix::toSlots(stereo, slots, 5, false);
        CHECK(slots[0] == 1 << 30);
        CHECK(slots[1] == -(1 << 30));
        CHECK(slots[2] == INT32_MAX);
        CHECK(slots[4] == INT32_MIN);
        CHECK(slots[6] == INT32_MAX);
        CHECK(slots[7] == INT32_MIN);
        AudioMix::toSlots(stereo, slots, 5, true);
        CHECK(slots[0] == 0);
        CHECK(slots[1] == 0);
        CHECK(slots[8] == 1 << 30);
        CHECK(slots[9] == 1 << 30);
        CHECK(std::fabs(AudioMix::gainOf(-600) - 0.501187f) < 1e-5f);
}

TEST_CASE("PcmStream: converts, waits for its start threshold, counts underruns, drains") {
        constexpr size_t  Period = 240;
        PcmStream         stream;
        PcmStream::Format format;
        format.format = SampleFormat::S16Le;
        format.channels = 1;
        format.rateHz = 48000;
        CHECK(stream.open(format, 0, 0, bmds::MemSpace::Psram) == Error::Invalid);
        format.channels = 3;
        CHECK(stream.open(format, 1000, 0, bmds::MemSpace::Psram) == Error::Invalid);
        format.channels = 1;
        REQUIRE(stream.open(format, 1000, 2 * Period, bmds::MemSpace::Psram).isOk());
        CHECK(stream.frameBytes() == 2);
        REQUIRE(stream.prepare(48000, Period).isOk());
        std::vector<float> mix(2 * Period, 0.0f);
        std::vector<float> scratch(2 * Period);
        // A ramp of 1.5 periods: short of the threshold, so nothing plays yet.
        std::vector<uint8_t> pcm;
        for (int i = 0; i < 360; ++i) {
                const auto v = static_cast<int16_t>(i * 64);
                pcm.push_back(static_cast<uint8_t>(v));
                pcm.push_back(static_cast<uint8_t>(v >> 8));
        }
        auto [taken, err] = stream.write(ByteSpan(pcm.data(), pcm.size()), true, nullptr);
        REQUIRE(err.isOk());
        CHECK(taken == pcm.size());
        CHECK(stream.queuedFrames() == 360);
        CHECK(stream.render(mix.data(), Period, scratch.data()));
        CHECK(std::all_of(mix.begin(), mix.end(), [](float v) { return v == 0.0f; }));
        // Past it: the frames come out as written, mono in both channels, with the gain.
        (void)stream.write(ByteSpan(pcm.data(), pcm.size()), true, nullptr);
        stream.setGain(0.5f);
        CHECK(stream.render(mix.data(), Period, scratch.data()));
        int wrong = 0;
        for (size_t i = 0; i < Period; ++i) {
                const float expected = 0.5f * static_cast<float>(i * 64) / 32768.0f;
                wrong += mix[2 * i] != expected || mix[2 * i + 1] != expected;
        }
        CHECK(wrong == 0);
        CHECK(stream.underruns() == 0);
        // 480 frames left; two periods take them, the third runs short: one underrun, however long.
        std::fill(mix.begin(), mix.end(), 0.0f);
        CHECK(stream.render(mix.data(), Period, scratch.data()));
        CHECK(stream.render(mix.data(), Period, scratch.data()));
        CHECK(stream.render(mix.data(), Period, scratch.data()));
        CHECK(stream.render(mix.data(), Period, scratch.data()));
        CHECK(stream.underruns() == 1);
        CHECK(stream.playedFrames() == 720);
        // Drained: with nothing left it is over.
        (void)stream.write(ByteSpan(pcm.data(), 100), true, nullptr);
        bmds::Interrupt *none = nullptr;
        CHECK(stream.drain(none).isOk()); // not attached: over already
        std::fill(mix.begin(), mix.end(), 0.0f);
        CHECK_FALSE(stream.render(mix.data(), Period, scratch.data())); // the 50 frames left: its last
        CHECK(mix[2 * 49] != 0.0f);
        CHECK(mix[2 * 50] == 0.0f);
        // A full ring takes what fits, and doesn't wait for a stream nobody plays.
        PcmStream small;
        REQUIRE(small.open(format, 128, 0, bmds::MemSpace::Psram).isOk());
        std::vector<uint8_t> lots(4096);
        auto [some, serr] = small.write(ByteSpan(lots.data(), lots.size()), false, nullptr);
        REQUIRE(serr.isOk());
        CHECK(some == 256); // 128 frames of 2 bytes: the ring's least
        CHECK(small.write(ByteSpan(lots.data(), lots.size()), false, nullptr).error() == Error::InvalidState);
}

TEST_CASE("PcmStream: resampled to the device's rate, and played out at the end") {
        constexpr size_t  Period = 240;
        PcmStream         stream;
        PcmStream::Format format;
        format.format = SampleFormat::F32Le;
        format.channels = 2;
        format.rateHz = 44100;
        REQUIRE(stream.open(format, 8192, 1, bmds::MemSpace::Psram).isOk());
        REQUIRE(stream.prepare(48000, Period).isOk());
        // 4410 frames (0.1 s) of a 1 kHz sine.
        std::vector<float> in(2 * 4410);
        for (size_t i = 0; i < 4410; ++i) in[2 * i] = in[2 * i + 1] = 0.5f * std::sin(2 * M_PI * 1000 * i / 44100);
        auto [taken, err] =
                stream.write(ByteSpan(reinterpret_cast<const uint8_t *>(in.data()), in.size() * 4), true, nullptr);
        REQUIRE(err.isOk());
        CHECK(taken == in.size() * 4);
        bmds::Interrupt *none = nullptr;
        (void)stream.drain(none);
        std::vector<float> mix(2 * Period);
        std::vector<float> scratch(2 * Period);
        std::vector<float> out;
        int                periods = 0;
        for (bool more = true; more;) {
                std::fill(mix.begin(), mix.end(), 0.0f);
                more = stream.render(mix.data(), Period, scratch.data());
                for (size_t i = 0; i < Period; ++i) out.push_back(mix[2 * i]);
                REQUIRE(++periods < 100);
        }
        // 0.1 s at 48 kHz, and the filter's tail, in whole periods.
        CHECK(out.size() >= 4800);
        CHECK(out.size() <= 4800 + 2 * Period);
        CHECK(stream.underruns() == 0);
        const SineFit fit = fitSine(out, 1000.0 / 48000, 400, 4400);
        CHECK(std::fabs(fit.amplitude - 0.5) < 0.005);
        CHECK(fit.restDb < -70);
}

TEST_CASE("ToneStream: a tone and a sound, made at the device's rate; cancelling fades") {
        constexpr size_t Period = 240;
        ToneStream       tone;
        tone.setTone(1000, 50, -600, SineTone::Right);
        REQUIRE(tone.prepare(48000, Period).isOk());
        std::vector<float> mix(2 * Period, 0.0f);
        int                periods = 0;
        float              leftPeak = 0, rightPeak = 0;
        for (bool more = true; more;) {
                more = tone.render(mix.data(), Period, nullptr);
                for (size_t i = 0; i < Period; ++i) {
                        leftPeak = std::max(leftPeak, std::fabs(mix[2 * i]));
                        rightPeak = std::max(rightPeak, std::fabs(mix[2 * i + 1]));
                }
                std::fill(mix.begin(), mix.end(), 0.0f);
                REQUIRE(++periods < 100);
        }
        CHECK(periods == 10); // 50 ms, its fade in the last
        CHECK(leftPeak == 0.0f);
        CHECK(rightPeak > 0.49f);
        char text[96];
        tone.describe(text, sizeof(text));
        CHECK(std::string(text) == "1000 Hz, -6.00 dBFS, right");
        ToneStream sound;
        sound.setSound(SystemSound::Bell);
        REQUIRE(sound.prepare(48000, Period).isOk());
        CHECK(sound.render(mix.data(), Period, nullptr));
        sound.cancel();
        periods = 0;
        while (sound.render(mix.data(), Period, nullptr)) REQUIRE(++periods < 3); // the fade: 3 ms
        sound.describe(text, sizeof(text));
        CHECK(std::string(text) == "bell");
}
