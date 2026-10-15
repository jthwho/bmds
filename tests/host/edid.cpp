/**
 * @file      edid.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * EDID parsing, on blocks built here byte by byte.
 */

#include <doctest.h>

#include <cstring>
#include <bmds/edid.h>
#include <bmds/videomodes.h>

using bmds::ByteSpan;
using bmds::Edid;
using bmds::Error;
using bmds::VideoTiming;

namespace {

        void seal(uint8_t (&block)[Edid::BlockBytes]) {
                uint8_t sum = 0;
                for (size_t i = 0; i < Edid::BlockBytes - 1; ++i) sum = static_cast<uint8_t>(sum + block[i]);
                block[Edid::BlockBytes - 1] = static_cast<uint8_t>(0x100 - sum);
                return;
        }

        // "DEL" 0xa07b, EDID 1.3, digital; 1920x1080 at 60 Hz (CEA 148.5 MHz)
        // preferred, a name, one extension.
        void baseBlock(uint8_t (&b)[Edid::BlockBytes]) {
                std::memset(b, 0, sizeof(b));
                const uint8_t header[8] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
                std::memcpy(b, header, sizeof(header));
                b[8] = 0x10; // D=4, E=5, L=12: 0x10ac
                b[9] = 0xac;
                b[10] = 0x7b;
                b[11] = 0xa0;
                b[18] = 1;
                b[19] = 3;
                b[20] = 0x80;
                const uint8_t dtd[18] = {0x02, 0x3a, 0x80, 0x18, 0x71, 0x38, 0x2d, 0x40, 0x58,
                                         0x2c, 0x45, 0x00, 0,    0,    0,    0,    0,    0x1e};
                std::memcpy(b + 54, dtd, sizeof(dtd));
                const uint8_t name[18] = {0,   0,   0,   0xfc, 0,   'D', 'E', 'L',  'L',
                                          ' ', 'U', '2', '4',  '1', '2', 'M', '\n', ' '};
                std::memcpy(b + 72, name, sizeof(name));
                // Range limits: 56-76 Hz, 30-83 kHz, 170 MHz.
                const uint8_t range[18] = {0, 0, 0, 0xfd, 0, 56, 76, 30, 83, 17, 0, '\n', ' ', ' ', ' ', ' ', ' ', ' '};
                std::memcpy(b + 90, range, sizeof(range));
                const uint8_t serial[18] = {0,   0,   0,   0xff, 0,   'A', 'B', 'C',  '1',
                                            '2', '3', ' ', ' ',  ' ', ' ', ' ', '\n', ' '};
                std::memcpy(b + 108, serial, sizeof(serial));
                b[16] = 12; // week 12 of 2014
                b[17] = 24;
                b[21] = 53; // 53 x 30 cm
                b[22] = 30;
                b[35] = 0x21; // 640x480@60, 800x600@60
                b[36] = 0x08; // 1024x768@60
                // Standard timings: 1280x1024@60 (5:4), 1920x1080@60 (16:9), then unused.
                const uint8_t std[16] = {0x81, 0x80, 0xd1, 0xc0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
                std::memcpy(b + 38, std, sizeof(std));
                b[126] = 1;
                seal(b);
                return;
        }

} // namespace

TEST_CASE("Edid: base block") {
        uint8_t b[Edid::BlockBytes];
        baseBlock(b);
        auto [e, err] = Edid::parse(ByteSpan(b, sizeof(b)));
        REQUIRE(err.isOk());
        CHECK(std::strcmp(e.maker, "DEL") == 0);
        CHECK(e.product == 0xa07b);
        CHECK(e.version == 1);
        CHECK(e.revision == 3);
        CHECK(e.digital);
        CHECK(std::strcmp(e.name, "DELL U2412M") == 0);
        CHECK(e.extensions == 1);
        CHECK_FALSE(e.hdmi);
        const VideoTiming &t = e.preferred;
        CHECK(t.pixelKhz == 148500);
        CHECK(t.hActive == 1920);
        CHECK(t.hFront == 88);
        CHECK(t.hSync == 44);
        CHECK(t.hBack == 148);
        CHECK(t.vActive == 1080);
        CHECK(t.vFront == 4);
        CHECK(t.vSync == 5);
        CHECK(t.vBack == 36);
        CHECK(t.hSyncHigh);
        CHECK(t.vSyncHigh);
        CHECK_FALSE(t.interlaced);
        CHECK(t.hTotal() == 2200);
        CHECK(t.vTotal() == 1125);
        CHECK(t.refreshMilliHz() == 60000);
        CHECK(e.detailedCount == 1);
        CHECK(e.detailed[0].hActive == 1920);
        CHECK(e.week == 12);
        CHECK(e.year == 2014);
        CHECK(e.widthCm == 53);
        CHECK(e.heightCm == 30);
        CHECK(std::strcmp(e.serial, "ABC123") == 0);
        REQUIRE(e.range.present);
        CHECK(e.range.minHz == 56);
        CHECK(e.range.maxHz == 76);
        CHECK(e.range.minKhz == 30);
        CHECK(e.range.maxKhz == 83);
        CHECK(e.range.maxPixelKhz == 170000);
        CHECK(e.established == 0x210800);
        Edid::Mode m;
        REQUIRE(Edid::establishedMode(21, m));
        CHECK((m.width == 640 && m.height == 480 && m.hz == 60));
        REQUIRE(Edid::establishedMode(11, m));
        CHECK((m.width == 1024 && m.height == 768 && m.hz == 60));
        CHECK_FALSE(Edid::establishedMode(6, m));
        REQUIRE(e.standardCount == 2);
        CHECK((e.standard[0].width == 1280 && e.standard[0].height == 1024 && e.standard[0].hz == 60));
        CHECK((e.standard[1].width == 1920 && e.standard[1].height == 1080 && e.standard[1].hz == 60));

        // Damage: the checksum, the header, the size.
        b[60] ^= 1;
        CHECK(Edid::parse(ByteSpan(b, sizeof(b))).error() == Error::CorruptData);
        baseBlock(b);
        b[0] = 1;
        seal(b);
        CHECK(Edid::parse(ByteSpan(b, sizeof(b))).error() == Error::CorruptData);
        CHECK(Edid::parse(ByteSpan(b, 64)).error() == Error::Invalid);
}

TEST_CASE("Edid: the CEA extension marks an HDMI sink") {
        uint8_t b[Edid::BlockBytes];
        baseBlock(b);
        auto [e, err] = Edid::parse(ByteSpan(b, sizeof(b)));
        REQUIRE(err.isOk());
        uint8_t x[Edid::BlockBytes] = {};
        x[0] = 0x02;
        x[1] = 0x03;
        x[2] = 4 + 1 + 6; // a video block of 0, then the vendor block
        x[4] = 0x40;      // video data block, empty
        x[5] = 0x65;      // vendor-specific, 5 bytes: OUI 00-0C-03, physical address 1.0.0.0
        x[6] = 0x03;
        x[7] = 0x0c;
        x[8] = 0x00;
        x[9] = 0x10;
        x[10] = 0x00;
        seal(x);
        CHECK(e.parseExtension(ByteSpan(x, sizeof(x))).isOk());
        CHECK(e.hdmi);
        CHECK(e.videoModeCount == 0);
        CHECK(e.extDetailedCount == 0);
        // Another vendor's block, or another kind of extension: still DVI.
        Edid other = e;
        other.hdmi = false;
        x[6] = 0x01;
        seal(x);
        CHECK(other.parseExtension(ByteSpan(x, sizeof(x))).isOk());
        CHECK_FALSE(other.hdmi);
        x[0] = 0x70;
        seal(x);
        CHECK(other.parseExtension(ByteSpan(x, sizeof(x))).isOk());
        CHECK_FALSE(other.hdmi);
        x[1] ^= 1;
        CHECK(other.parseExtension(ByteSpan(x, sizeof(x))) == Error::CorruptData);
}

TEST_CASE("Edid: the CEA extension's video modes and detailed timings") {
        uint8_t b[Edid::BlockBytes];
        baseBlock(b);
        auto [e, err] = Edid::parse(ByteSpan(b, sizeof(b)));
        REQUIRE(err.isOk());
        uint8_t x[Edid::BlockBytes] = {};
        x[0] = 0x02;
        x[1] = 0x03;
        x[2] = 4 + 4; // a video block of 3, then the detailed timings
        x[4] = 0x43;  // video data block, 3 descriptors
        x[5] = 0x90;  // VIC 16 (1920x1080p60), native
        x[6] = 0x04;  // VIC 4 (1280x720p60)
        x[7] = 0x75;  // VIC 117: not named here
        const uint8_t dtd[18] = {0x01, 0x1d, 0x00, 0x72, 0x51, 0xd0, 0x1e, 0x20, 0x6e,
                                 0x28, 0x55, 0x00, 0,    0,    0,    0,    0,    0x1e}; // 1280x720p60, 74.25 MHz
        std::memcpy(x + 8, dtd, sizeof(dtd));
        seal(x);
        REQUIRE(e.parseExtension(ByteSpan(x, sizeof(x))).isOk());
        REQUIRE(e.videoModeCount == 3);
        Edid::Mode m;
        uint8_t    vic = 0;
        bool       native = false;
        REQUIRE(Edid::videoMode(e.videoModes[0], m, vic, native));
        CHECK(vic == 16);
        CHECK(native);
        CHECK((m.width == 1920 && m.height == 1080 && m.hz == 60));
        REQUIRE(Edid::videoMode(e.videoModes[1], m, vic, native));
        CHECK(vic == 4);
        CHECK_FALSE(native);
        CHECK_FALSE(Edid::videoMode(e.videoModes[2], m, vic, native));
        CHECK(vic == 117);
        REQUIRE(e.extDetailedCount == 1);
        CHECK(e.extDetailed[0].hActive == 1280);
        CHECK(e.extDetailed[0].vActive == 720);
        CHECK(e.extDetailed[0].pixelKhz == 74250);
}

TEST_CASE("VideoModes: standard timings, CVT reduced blanking, parsing") {
        using bmds::VideoModes::parse;
        auto [cea, err] = parse("1280x720@60");
        REQUIRE(err.isOk());
        CHECK(cea.pixelKhz == 74250);
        CHECK(cea.hTotal() == 1650);
        CHECK(cea.vTotal() == 750);
        CHECK(cea.refreshMilliHz() == 60000);
        CHECK(parse("1024x768@60").value().pixelKhz == 65000);
        CHECK(parse("1920x1080@30").value().hTotal() == 2200);
        // CVT-RB: 1080p60 is VESA's 2080 x 1111 (138.5 MHz rounded; exact here).
        const VideoTiming rb60 = parse("1920x1080@60").value();
        CHECK(rb60.hTotal() == 2080);
        CHECK(rb60.vTotal() == 1111);
        CHECK(rb60.pixelKhz == 138652);
        CHECK(rb60.hSyncHigh);
        CHECK_FALSE(rb60.vSyncHigh);
        const VideoTiming rb50 = parse("1920x1080@50").value();
        CHECK(rb50.vTotal() == 1080 + 3 + 5 + 18);
        CHECK(rb50.refreshMilliHz() >= 49990);
        CHECK(rb50.refreshMilliHz() <= 50010);
        // Bad text and values.
        CHECK(parse("1280x720").error() == Error::Invalid);
        CHECK(parse("1280x720@").error() == Error::Invalid);
        CHECK(parse("x720@60").error() == Error::Invalid);
        CHECK(parse("1280x720@60hz").error() == Error::Invalid);
        CHECK(parse("1280x0@60").error() == Error::Invalid);
        CHECK(parse("1280x720@300").error() == Error::Invalid);
}

TEST_CASE("VideoModes: fitting a mode to the clocks the PLLs make") {
        using namespace bmds::VideoModes;
        constexpr uint32_t Plls[] = {240'000, 160'000};
        // CEA 720p60 (74.25 MHz) at 80 MHz: wider front porch, same lines, ~60 Hz.
        const VideoTiming fit = fitToClock(parse("1280x720@60").value(), Plls, 2, 0, 125'000).value();
        CHECK(fit.pixelKhz == 80'000);
        CHECK(fit.vTotal() == 750);
        CHECK(fit.hTotal() == 1778);
        CHECK(fit.hFront == 110 + 128);
        CHECK(fit.refreshMilliHz() >= 59'990);
        CHECK(fit.refreshMilliHz() <= 60'010);
        // An exact clock stays as it is.
        const VideoTiming esp = {80'000, 1280, 408, 32, 80, 720, 3, 5, 13, true, false, false};
        CHECK(fitToClock(esp, Plls, 2, 0, 125'000).value().hTotal() == esp.hTotal());
        // 1080p50 (CVT-RB, 115 MHz) at 120 MHz.
        const VideoTiming fhd = fitToClock(parse("1920x1080@50").value(), Plls, 2, 0, 125'000).value();
        CHECK(fhd.pixelKhz == 120'000);
        CHECK(fhd.hTotal() <= 4095);
        // Beyond the link: no clock.
        CHECK(fitToClock(parse("1920x1080@60").value(), Plls, 2, 0, 125'000).error() == Error::NotSupported);
}

TEST_CASE("Edid::takes: a listed mode, or one inside the range limits") {
        Edid e;
        e.established = 1u << 23; // 720x400 at 70 Hz
        auto [vga, verr] = bmds::VideoModes::timing(720, 400, 70);
        REQUIRE(verr.isOk());
        CHECK(e.takes(vga));
        auto [m13h, merr] = bmds::VideoModes::timing(640, 400, 70);
        REQUIRE(merr.isOk());
        CHECK(m13h.pixelKhz == 25'175);
        CHECK_FALSE(e.takes(m13h)); // not listed, no limits
        e.range.present = true;     // the VE248's: 50-76 Hz, 30-83 kHz, 170 MHz
        e.range.minHz = 50;
        e.range.maxHz = 76;
        e.range.minKhz = 30;
        e.range.maxKhz = 83;
        e.range.maxPixelKhz = 170'000;
        CHECK(e.takes(m13h)); // 70.1 Hz, 31.5 kHz
        e.range.minKhz = 32;
        CHECK_FALSE(e.takes(m13h));
}

TEST_CASE("VideoModes::fullScreen: a VGA mode the picture fills at its shape") {
        Edid e;
        e.range.present = true;
        e.range.minHz = 50;
        e.range.maxHz = 76;
        e.range.minKhz = 30;
        e.range.maxKhz = 83;
        e.range.maxPixelKhz = 170'000;
        // Doom: 320x200 shown at 4:3: mode 13h's 640x400 at 70 Hz.
        auto [doom, derr] = bmds::VideoModes::fullScreen(320, 200, 4, 3, e);
        REQUIRE(derr.isOk());
        CHECK(doom.hActive == 640);
        CHECK(doom.vActive == 400);
        CHECK((doom.refreshMilliHz() + 500) / 1000 == 70);
        // Square pixels at 4:3 fill 640x480.
        auto [qvga, qerr] = bmds::VideoModes::fullScreen(320, 240, 0, 0, e);
        REQUIRE(qerr.isOk());
        CHECK(qvga.vActive == 480);
        // Other shapes stay on the desktop's mode.
        CHECK(bmds::VideoModes::fullScreen(320, 200, 0, 0, e).error() == Error::NotFound); // 16:10
        CHECK(bmds::VideoModes::fullScreen(640, 360, 0, 0, e).error() == Error::NotFound); // Blocks, 16:9
        // A monitor that takes none of them.
        Edid none;
        CHECK(bmds::VideoModes::fullScreen(320, 200, 4, 3, none).error() == Error::NotFound);
}
