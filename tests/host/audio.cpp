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
#include <bmds/es8311regs.h>
#include <bmds/sinetone.h>

using bmds::AudioControl;
using bmds::Error;
using bmds::SineTone;
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
        std::vector<int32_t> buf(2 * Rate);
        tone.fill(buf.data(), Rate);
        // Right silent; left crosses zero upward 1000 times a second.
        int32_t peak = 0;
        int     upward = 0;
        int     rightSamples = 0;
        for (uint32_t i = 0; i < Rate; ++i) {
                rightSamples += buf[2 * i + 1] != 0;
                peak = std::max(peak, std::abs(buf[2 * i]));
                if (i > 0 && buf[2 * (i - 1)] < 0 && buf[2 * i] >= 0) ++upward;
        }
        CHECK(rightSamples == 0);
        CHECK(upward >= 999);
        CHECK(upward <= 1000);
        // -6 dBFS: half of full scale, within a little.
        const double expected = 2147483520.0 * std::pow(10.0, -6.0 / 20.0);
        CHECK(std::abs(peak - expected) / expected < 0.001);
        // It faded in: the first sample is near zero, and early ones small.
        CHECK(std::abs(buf[0]) < expected * 0.01);
        tone.fadeOut();
        CHECK(!tone.isFinished());
        tone.fill(buf.data(), Rate / 100); // 10 ms: past the 5 ms fade
        CHECK(tone.isFinished());
        CHECK(std::abs(buf[2 * (Rate / 100 - 1)]) == 0);
}
